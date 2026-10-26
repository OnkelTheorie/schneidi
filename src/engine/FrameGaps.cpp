#include "engine/FrameGaps.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSaveFile>
#include <QStandardPaths>

#include <memory>
#include <vector>

namespace FrameGaps {

namespace {

constexpr int kVersion = 1; // bump when the rule in Stutter::fromTimestamps changes (old disk entries are ignored)

QMutex g_mutex;
QHash<QString, std::optional<Stutter::Scan>>& memory()
{
    static QHash<QString, std::optional<Stutter::Scan>> cache;
    return cache;
}

QString keyOf(const QFileInfo& fi)
{
    const QString id = QString("%1|%2|%3|%4")
                           .arg(fi.absoluteFilePath())
                           .arg(fi.size())
                           .arg(fi.lastModified().toMSecsSinceEpoch())
                           .arg(kVersion);
    return QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString cacheFile(const QString& key)
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/gaps/" + key + ".json";
}

std::optional<std::optional<Stutter::Scan>> loadDisk(const QString& key)
{
    QFile f(cacheFile(key));
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o["version"].toInt() != kVersion) return std::nullopt;
    if (!o["video"].toBool(true)) return std::optional<Stutter::Scan>{};
    Stutter::Scan s;
    s.frameDuration = o["frame_duration"].toDouble();
    s.duration = o["duration"].toDouble();
    s.frames = o["frames"].toInt();
    for (const QJsonValue& v : o["jumps"].toArray()) {
        const QJsonArray j = v.toArray();
        s.jumps << Stutter::Jump{j[0].toDouble(), j[1].toDouble(), j[2].toInt()};
    }
    return std::optional<Stutter::Scan>{s};
}

void saveDisk(const QString& key, const std::optional<Stutter::Scan>& s)
{
    QJsonObject o{{"version", kVersion}, {"video", bool(s)}};
    if (s) {
        QJsonArray jumps;
        for (const Stutter::Jump& j : s->jumps) jumps << QJsonArray{j.at, j.length, j.missing};
        o["frame_duration"] = s->frameDuration;
        o["duration"] = s->duration;
        o["frames"] = s->frames;
        o["jumps"] = jumps;
    }
    const QString file = cacheFile(key);
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile out(file);
    if (out.open(QIODevice::WriteOnly) && out.write(QJsonDocument(o).toJson(QJsonDocument::Compact)) >= 0) out.commit();
}

// The presentation times of the first video stream's packets, counted like the own decoder (engine/video/Reader.cpp):
// frame 0 = the first key frame (0 when its DTS is negative, MP4 with B-frames)
std::optional<Stutter::Scan> read(const QString& path)
{
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path.toUtf8().constData(), nullptr, nullptr) < 0) return std::nullopt;
    const std::unique_ptr<AVFormatContext, void (*)(AVFormatContext*)> fmt(raw, [](AVFormatContext* f) {
        avformat_close_input(&f);
    });
    const auto videoStream = [&]() -> int {
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVStream* st = fmt->streams[i];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC))
                return int(i);
        }
        return -1;
    };
    int index = videoStream();
    if (index < 0) { // streams that only show up while reading (MPEG-TS)
        if (avformat_find_stream_info(fmt.get(), nullptr) < 0) return std::nullopt;
        index = videoStream();
    }
    if (index < 0) return std::nullopt;
    // Image formats (a still, an image sequence) have no recording time line
    if (fmt->iformat && QByteArray(fmt->iformat->name).contains("image2")) return std::nullopt;
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (int(i) != index) fmt->streams[i]->discard = AVDISCARD_ALL; // not even read where the format allows it
    const AVRational tb = fmt->streams[index]->time_base;

    std::vector<int64_t> stamps;
    int64_t origin = AV_NOPTS_VALUE;
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(fmt.get(), pkt) >= 0) {
        if (pkt->stream_index == index) {
            const int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            if (ts != AV_NOPTS_VALUE) {
                stamps.push_back(ts);
                if (origin == AV_NOPTS_VALUE && (pkt->flags & AV_PKT_FLAG_KEY))
                    origin = pkt->dts != AV_NOPTS_VALUE && pkt->dts < 0 ? 0 : ts;
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (stamps.size() < 3) return std::nullopt; // a still in a video container
    if (origin == AV_NOPTS_VALUE) origin = stamps.front();
    std::vector<double> times(stamps.size());
    for (size_t i = 0; i < stamps.size(); ++i) times[i] = double(stamps[i] - origin) * av_q2d(tb);
    return Stutter::fromTimestamps(std::move(times));
}

} // namespace

std::optional<Stutter::Scan> scan(const QString& path)
{
    const QFileInfo fi(path);
    if (!fi.isFile()) return std::nullopt;
    const QString key = keyOf(fi);
    {
        QMutexLocker lock(&g_mutex);
        if (const auto it = memory().constFind(key); it != memory().constEnd()) return *it;
    }
    std::optional<Stutter::Scan> s;
    if (const auto disk = loadDisk(key)) {
        s = *disk;
    } else {
        s = read(fi.absoluteFilePath());
        saveDisk(key, s);
    }
    QMutexLocker lock(&g_mutex);
    memory().insert(key, s);
    return s;
}

void clearMemoryCache()
{
    QMutexLocker lock(&g_mutex);
    memory().clear();
}

} // namespace FrameGaps
