// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/MediaCache.h"
#include "core/AppPaths.h"
#include "engine/picture/MediaStills.h"

#include "engine/ClipFormat.h"
#include "engine/ProxyManager.h"
#include "engine/audio/Reader.h"
#include "engine/Background.h"
#include "engine/FileStamp.h"
#include "engine/GradedThumbs.h"
#include "engine/video/Thumbnails.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
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

QString peaksDir()
{
    const QString dir = AppPaths::writable(QStandardPaths::CacheLocation) + "/peaks";
    QDir().mkpath(dir);
    return dir;
}

QString fileId(const QString& path, const FrameRate& rate)
{
    const QFileInfo fi(path);
    return QString("%1|%2|%3|%4|%5")
        .arg(fi.absoluteFilePath())
        .arg(fi.size())
        .arg(fi.lastModified().toMSecsSinceEpoch())
        .arg(QString("%1/%2").arg(rate.num).arg(rate.den))
        .arg(Waveform::kBucketsPerFrame);
}

// Disk cache for unrouted waveforms: key from path, size and modification time. Further streams with a suffix so that
// existing waveforms (stream 0) stay valid; "own|": the sound as the own audio engine places it (entries without it
// came from the sound path before 0.10). Routed waveforms (0.23 multichannel: "c<channels>|" entries) are no longer
// written here but combined from the per-channel file (channelPeaksFile); their old files are simply never read.
QString waveCacheFile(const QString& path, int stream, const FrameRate& rate)
{
    const QString id = QStringLiteral("own|") + (stream > 0 ? QString("s%1|").arg(stream) : QString()) + fileId(path, rate);
    return peaksDir() + "/" + QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex() + ".peaks";
}

constexpr quint32 kWaveMagic = 0x53574631;    // "SWF1": one peak array (unrouted waveform)
constexpr quint32 kChannelMagic = 0x53574632; // "SWF2": per-channel peaks of a stream
constexpr qint32 kChannelVersion = 1;
constexpr int kMaxChannels = 256;

// Peak of a sample (0..1) as 0..255, as the s16 path did
quint8 peakValue(float peak) { return quint8(std::min(255, int(std::min(peak, 1.f) * 32767.f) >> 7)); }

bool loadWave(const QString& file, Waveform& w)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QDataStream in(&f);
    quint32 magic = 0;
    qint32 frames = 0;
    QByteArray data;
    in >> magic >> frames >> data;
    if (magic != kWaveMagic || in.status() != QDataStream::Ok
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
    out << kWaveMagic << qint32(frames)
        << QByteArray(reinterpret_cast<const char*>(base.constData()), base.size());
    f.commit();
}

// Per-channel peaks: magic, version, channels, frames, then channel after channel (frames * kBucketsPerFrame each).
// Anything else (an SWF1 file, another version, a short file) is not read: the peaks are decoded again.
bool loadChannelPeaks(const QString& file, int& frames, QVector<QVector<quint8>>& base)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QDataStream in(&f);
    quint32 magic = 0;
    qint32 version = 0, channels = 0, count = 0;
    in >> magic >> version;
    if (magic != kChannelMagic || version != kChannelVersion) return false;
    in >> channels >> count;
    if (in.status() != QDataStream::Ok || channels < 1 || channels > kMaxChannels || count < 0) return false;
    QByteArray data;
    in >> data;
    const qint64 per = qint64(count) * Waveform::kBucketsPerFrame;
    if (in.status() != QDataStream::Ok || data.size() != per * channels) return false;
    base.clear();
    for (int c = 0; c < channels; ++c) {
        const char* from = data.constData() + per * c;
        base << QVector<quint8>(from, from + per);
    }
    frames = count;
    return true;
}

void saveChannelPeaks(const QString& file, int frames, const QVector<QVector<quint8>>& base)
{
    QByteArray data;
    data.reserve(qsizetype(base.size()) * frames * Waveform::kBucketsPerFrame);
    for (const QVector<quint8>& ch : base) data.append(reinterpret_cast<const char*>(ch.constData()), ch.size());
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly)) return;
    QDataStream out(&f);
    out << kChannelMagic << kChannelVersion << qint32(base.size()) << qint32(frames) << data;
    f.commit();
}

// The waveform of the routed channels `pick` (Audio::Reader::Options::pick: the first two count; one alone plays in the
// middle at -3 dB each side, so its peak too) from per-channel peaks, frames [0, frames). A channel the stream lacks
// is silent.
QVector<quint8> combinePeaks(const QVector<QVector<quint8>>& base, int frames, QVector<int> pick)
{
    constexpr int N = Waveform::kBucketsPerFrame;
    if (pick.size() > 2) pick.resize(2);
    QVector<quint8> out(qsizetype(frames) * N, 0);
    const bool middle = pick.size() == 1;
    for (int ch : pick) {
        if (ch < 0 || ch >= base.size()) continue;
        const quint8* in = base[ch].constData();
        const qsizetype n = std::min<qsizetype>(out.size(), base[ch].size());
        for (qsizetype i = 0; i < n; ++i) {
            const int v = middle ? (in[i] * 46341 + 32768) >> 16 : in[i]; // 46341 / 65536 = 0.70711
            out[i] = quint8(std::max<int>(out[i], v));
        }
    }
    return out;
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
    m_graded = new GradedThumbs(this);
    connect(m_graded, &GradedThumbs::updated, this, &MediaCache::updated);
    // A media file overwritten under its name (seen by any reader, any thread): drop it all on the UI thread
    m_stampObserver = FileStamp::addObserver([this](const QString& path) {
        QMetaObject::invokeMethod(this, [this, path] { invalidateFile(path); }, Qt::QueuedConnection);
    });
    m_thumbThread = QThread::create([this] {
        Background::lowerThisThread(Background::Level::Low);
        thumbLoop();
    });
    m_stripThread = QThread::create([this] {
        Background::lowerThisThread(Background::Level::Low); // also the decoder threads it starts (inherit the nice value)
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
    FileStamp::removeObserver(m_stampObserver);
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
    FileStamp::lookLater(path); // an overwritten file is noticed (throttled, on a pool thread: no stat here)
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
        m_channelPeaks.clear();
        m_waveJobs.clear();
        m_waveRequested.clear();
    }
    m_graded->clear();
    emit updated();
}

void MediaCache::invalidateFile(const QString& path)
{
    {
        QMutexLocker lock(&m_mutex);
        ++m_fileGeneration[path]; // results of it under way are dropped
        const QString thumbPrefix = path + '#';
        const QList<QString> thumbs = m_thumbs.keys();
        for (const QString& k : thumbs)
            if (k.startsWith(thumbPrefix)) m_thumbs.remove(k);
        std::erase_if(m_thumbJobs, [&](const ThumbJob& j) { return j.path == path; });
        for (auto it = m_thumbPending.begin(); it != m_thumbPending.end();)
            it = it->startsWith(thumbPrefix) ? m_thumbPending.erase(it) : std::next(it);
        m_keys.remove(path);
        std::erase(m_stripJobs, path);
        const auto ofFile = [&](const QString& k) { return k == path || k.startsWith(path + QChar(0)); };
        for (auto it = m_waves.begin(); it != m_waves.end();) it = ofFile(it.key()) ? m_waves.erase(it) : std::next(it);
        for (auto it = m_channelPeaks.begin(); it != m_channelPeaks.end();)
            it = ofFile(it.key()) ? m_channelPeaks.erase(it) : std::next(it);
        std::erase_if(m_waveJobs, ofFile);
        for (auto it = m_waveRequested.begin(); it != m_waveRequested.end();)
            it = ofFile(*it) ? m_waveRequested.erase(it) : std::next(it);
    }
    emit updated(); // the timeline asks again
}

std::shared_ptr<const Waveform> MediaCache::waveform(const QString& path, int stream, const QVector<int>& channels)
{
    // Key: path, for further streams with an appended number (\0 does not occur in paths), for routed channels also
    // "|<channels>" ("|2,3")
    QStringList picked;
    for (int c : channels) picked << QString::number(c);
    QString key = path;
    if (stream > 0 || !picked.isEmpty()) key += QChar(0) + QString::number(stream);
    if (!picked.isEmpty()) key += '|' + picked.join(',');
    FileStamp::lookLater(path); // as thumbnail()
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
    int generation = -1;
    // Keeps its sources open (opening a file is more expensive than seeking)
    std::unique_ptr<Picture::MediaStills> own;

    for (;;) {
        ThumbJob job;
        ProjectFormat format;
        int gen = 0, fileGen = 0;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_thumbJobs.empty()) m_thumbCond.wait(&m_mutex);
            if (m_quit) return;
            job = m_thumbJobs.front();
            m_thumbJobs.pop_front();
            format = m_format;
            gen = m_generation;
            fileGen = m_fileGeneration.value(job.path);
        }
        Background::yieldToForeground();
        if (gen != generation) { // new project format
            own = std::make_unique<Picture::MediaStills>(format, &ProxyManager::decodeSource, 8);
            generation = gen;
        }

        QImage img = own->frame(job.path, job.frame, thumbSize(format));
        if (!img.isNull()) img = img.convertToFormat(QImage::Format_RGB32); // faster to draw

        {
            QMutexLocker lock(&m_mutex);
            if (stale(gen, job.path, fileGen)) continue; // new format or file in the meantime
            const QString key = thumbKey(job.path, job.frame);
            m_thumbPending.remove(key);
            if (!img.isNull()) m_thumbs.insert(key, new QImage(img), int(img.sizeInBytes() / 1024) + 1);
        }
        if (!img.isNull()) emit updated();
    }
}

void MediaCache::stripLoop()
{
    // Decoder threads: independent key frames decode in parallel; more would only hold more pictures in memory
    const int threads = std::clamp(QThread::idealThreadCount() / 2, 1, 4);
    for (;;) {
        QString path;
        ProjectFormat format;
        int gen = 0, fileGen = 0;
        {
            QMutexLocker lock(&m_mutex);
            while (!m_quit && m_stripJobs.empty()) m_stripCond.wait(&m_mutex);
            if (m_quit) return;
            path = m_stripJobs.front();
            m_stripJobs.pop_front();
            format = m_format;
            gen = m_generation;
            fileGen = m_fileGeneration.value(path);
        }
        Background::yieldToForeground();
        const double fps = format.rate.fps();
        // Only key frames, upright, square pixels, fitted into the project format centered with black borders like the timeline
        int count = 0;
        bool aborted = false;
        QElapsedTimer sinceUpdate;
        sinceUpdate.start();
        Video::keyFrames(ProxyManager::decodeSource(path), {thumbSize(format), threads}, [&](double seconds, const QImage& img) {
            const int frame = std::max(0, int(std::lround(seconds * fps)));
            {
                QMutexLocker lock(&m_mutex);
                if (m_quit || stale(gen, path, fileGen)) {
                    aborted = true;
                    return false;
                }
                KeyIndex& ki = m_keys[path];
                const auto pos = std::lower_bound(ki.frames.begin(), ki.frames.end(), frame);
                if (pos == ki.frames.end() || *pos != frame) ki.frames.insert(pos, frame);
                m_thumbs.insert(thumbKey(path, frame), new QImage(img), int(img.sizeInBytes() / 1024) + 1);
            }
            // The first picture at once, then at most ten times a second (each update repaints the timeline)
            if (count++ == 0 || sinceUpdate.elapsed() >= 100) {
                emit updated();
                sinceUpdate.restart();
            }
            Background::yieldToForeground();
            return true;
        });
        if (aborted) continue;
        {
            QMutexLocker lock(&m_mutex);
            if (stale(gen, path, fileGen)) continue;
            KeyIndex& ki = m_keys[path];
            ki.done = true;
            ki.failed = count == 0; // no video / unreadable -> exact frames through the decoder
        }
        emit updated();
    }
}

void MediaCache::waveLoop()
{
    constexpr int N = Waveform::kBucketsPerFrame;

    for (;;) {
        QString key, path, channels; // channels: routed ones of the stream, "2,3" (empty = all)
        int stream = 0;
        ProjectFormat format;
        int gen = 0, fileGen = 0;
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
                const QString rest = key.mid(z + 1);
                stream = rest.section('|', 0, 0).toInt();
                channels = rest.section('|', 1);
            }
            format = m_format;
            gen = m_generation;
            fileGen = m_fileGeneration.value(path);
        }
        if (!channels.isEmpty()) {
            QVector<int> pick;
            for (const QString& c : channels.split(',', Qt::SkipEmptyParts)) pick << c.toInt();
            routedWave(key, path, stream, pick, format, gen, fileGen);
            {
                QMutexLocker lock(&m_mutex);
                if (m_quit) return;
            }
            continue;
        }
        // Waveform counts frames in the project frame rate
        auto publish = [&](std::shared_ptr<const Waveform> w) {
            {
                QMutexLocker lock(&m_mutex);
                if (stale(gen, path, fileGen)) return; // new format or file in the meantime
                m_waves.insert(key, std::move(w));
            }
            emit updated();
        };

        const QString cacheFile = waveCacheFile(path, stream, format.rate);
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
        // The stream as the mixer reads it (stereo, 48 kHz), frame i = samples of timeline frame i
        std::unique_ptr<Audio::Reader> reader;
        std::vector<float> buf[2];
        int64_t bufFrom = 0, bufTo = 0;
        {
            constexpr int kRate = 48000;
            reader = Audio::Reader::open(path, stream, Audio::Reader::Options{kRate, 2});
            if (reader) ++m_waveDecodes;
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
                    peaks[k] = peakValue(peak);
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
                if (m_quit || stale(gen, path, fileGen)) {
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

QString MediaCache::channelPeaksFile(const QString& path, int stream, const FrameRate& rate)
{
    const QString id = QString("own|chan%1|s%2|").arg(kChannelVersion).arg(stream) + fileId(path, rate);
    return peaksDir() + "/" + QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex() + ".peaks";
}

void MediaCache::routedWave(const QString& key, const QString& path, int stream, const QVector<int>& pick,
                            const ProjectFormat& format, int gen, int fileGen)
{
    constexpr int N = Waveform::kBucketsPerFrame;
    const QString streamKey = path + QChar(0) + QString::number(stream);
    auto publish = [&](const QVector<QVector<quint8>>& base, int frames, bool complete,
                       std::shared_ptr<const ChannelPeaks> keep) {
        auto w = std::make_shared<Waveform>();
        w->levels = buildLevels(combinePeaks(base, frames, pick));
        w->frames = frames;
        w->complete = complete;
        {
            QMutexLocker lock(&m_mutex);
            if (stale(gen, path, fileGen)) return false; // new format or file in the meantime
            if (keep) m_channelPeaks.insert(streamKey, std::move(keep));
            m_waves.insert(key, std::move(w));
        }
        emit updated();
        return true;
    };

    std::shared_ptr<const ChannelPeaks> known;
    {
        QMutexLocker lock(&m_mutex);
        known = m_channelPeaks.value(streamKey);
    }
    if (known) { // another routing of this stream was built already: no decode, no disk
        publish(known->base, known->frames, true, nullptr);
        return;
    }
    const QString cacheFile = channelPeaksFile(path, stream, format.rate);
    {
        auto loaded = std::make_shared<ChannelPeaks>();
        if (loadChannelPeaks(cacheFile, loaded->frames, loaded->base)) {
            publish(loaded->base, loaded->frames, true, loaded);
            return;
        }
    }

    // Every channel of the stream as it comes (48 kHz), frame i = samples of timeline frame i (as the stereo path)
    constexpr int kRate = 48000;
    const std::unique_ptr<Audio::Reader> reader = Audio::Reader::open(path, stream, Audio::Reader::Options{kRate, 0});
    auto result = std::make_shared<ChannelPeaks>();
    if (!reader) {
        publish(result->base, 0, true, result);
        return;
    }
    ++m_waveDecodes;
    const FrameRate fps = format.rate;
    const auto frameStart = [fps](int64_t f) {
        const int64_t n = f * kRate * fps.den;
        return (2 * n + fps.num) / (2 * int64_t(fps.num)); // = Audio::Mixer::frameStart (f >= 0)
    };
    const int length =
        int(std::ceil(double(std::max<int64_t>(0, reader->info().end)) * fps.num / (double(kRate) * fps.den)));
    // The reader decides its channel count at the first decoded frame (= the stream's): room for any count it may
    // write, the channels past the stream's own go to a scratch buffer
    const int chans = std::clamp(reader->info().sourceChannels, 1, kMaxChannels);
    QVector<QVector<quint8>> base(chans, QVector<quint8>(qsizetype(std::max(0, length)) * N, 0));
    if (length <= 0) {
        result->base = base;
        publish(result->base, 0, true, result);
        return;
    }
    std::vector<std::vector<float>> buf(static_cast<size_t>(chans));
    std::vector<float> scratch;
    std::vector<float*> out(size_t(std::max(chans, kMaxChannels)));
    int64_t bufFrom = 0, bufTo = 0;
    QElapsedTimer sincePublish;
    sincePublish.start();
    for (int i = 0; i < length; ++i) {
        if (i % 256 == 0) {
            QMutexLocker lock(&m_mutex);
            if (m_quit || stale(gen, path, fileGen)) return; // format change or quit: the result no longer applies
        }
        const int64_t a = frameStart(i), b = frameStart(i + 1);
        if (b > bufTo) { // read about a second ahead
            bufFrom = a;
            bufTo = std::max(b, frameStart(i + 25));
            const size_t n = size_t(bufTo - bufFrom);
            scratch.resize(n);
            for (size_t c = 0; c < out.size(); ++c) {
                if (c < buf.size()) {
                    buf[c].assign(n, 0.f);
                    out[c] = buf[c].data();
                } else {
                    out[c] = scratch.data();
                }
            }
            reader->read(bufFrom, int(n), out.data());
        }
        const int samples = int(b - a);
        for (int c = 0; c < chans; ++c) {
            const float* src = buf[size_t(c)].data() + (a - bufFrom);
            quint8* peaks = base[c].data() + qsizetype(i) * N;
            for (int k = 0; k < N; ++k) {
                const int s0 = samples * k / N, s1 = samples * (k + 1) / N;
                float peak = 0.f;
                for (int s = s0; s < s1; ++s) peak = std::max(peak, std::abs(src[s]));
                peaks[k] = peakValue(peak);
            }
        }
        // Intermediate states so that long files do not stay empty forever
        if (sincePublish.elapsed() > 700) {
            publish(base, i + 1, false, nullptr);
            sincePublish.restart();
        }
    }
    result->frames = length;
    result->base = std::move(base);
    if (publish(result->base, length, true, result)) saveChannelPeaks(cacheFile, length, result->base);
}
