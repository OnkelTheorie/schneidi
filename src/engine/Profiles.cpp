// SPDX-License-Identifier: AGPL-3.0-only
#include "engine/Profiles.h"
#include "engine/Bundle.h"
#include "engine/MediaProbe.h"
#include "engine/audio/Path.h"
#include "engine/video/Producer.h"
#include "engine/video/Reader.h"
#include "engine/video/Still.h"

#include <Mlt.h>
#include <QByteArray>
#include <QFileInfo>
#include <QProcess>

extern "C" {
#include <libavformat/avformat.h>
}

#include <cmath>
#include <memory>
#include <optional>
#include <numeric>

namespace {
// Absolute stream indices (meta.media.N) of the audio streams
QVector<int> audioIndices(Mlt::Producer& p)
{
    QVector<int> out;
    const int n = p.get_int("meta.media.nb_streams");
    for (int i = 0; i < n; ++i)
        if (qstrcmp(p.get(QString("meta.media.%1.stream.type").arg(i).toUtf8().constData()), "audio") == 0) out << i;
    return out;
}
} // namespace

QVector<AudioStreamInfo> audioStreamsOf(Mlt::Producer& p)
{
    QVector<AudioStreamInfo> out;
    for (int i : audioIndices(p)) {
        AudioStreamInfo s;
        s.channels = std::max(1, p.get_int(QString("meta.media.%1.codec.channels").arg(i).toUtf8().constData()));
        s.title = QString::fromUtf8(p.get(QString("meta.attr.%1.stream.title.markup").arg(i).toUtf8().constData())).trimmed();
        out << s;
    }
    return out;
}

void selectAudioStream(Mlt::Producer& p, int stream)
{
    if (stream <= 0 && p.get_int("audio_index") < 0) return; // no audio
    const QVector<int> idx = audioIndices(p);
    if (idx.isEmpty() && stream <= 0) return; // metadata missing: leave the default stream
    p.set("audio_index", stream >= 0 && stream < idx.size() ? idx[stream] : -1);
}

std::unique_ptr<Mlt::Profile> makeProfile(const ProjectFormat& f)
{
    auto p = std::make_unique<Mlt::Profile>("atsc_1080p_25"); // basis, all values get overwritten
    const int g = std::max(1, std::gcd(f.width, f.height));
    p->set_width(f.width);
    p->set_height(f.height);
    p->set_frame_rate(f.rate.num, f.rate.den);
    p->set_sample_aspect(1, 1);
    p->set_display_aspect(f.width / g, f.height / g);
    p->set_progressive(1);
    p->set_colorspace(709);
    p->set_explicit(1);
    return p;
}

ProducerFactory::ProducerFactory(Mlt::Profile& base) : m_base(base) {}
ProducerFactory::~ProducerFactory() = default;

namespace {

// A media file the own decoder may read (not timewarp:…, color:…, still images)
bool forOwnDecoder(const QString& resource)
{
    if (!Video::ownDecoder() || !Audio::ownPath()) return false;
    static const QStringList stills{"png", "jpg", "jpeg", "bmp", "gif", "webp", "tif", "tiff", "svg", "exr", "tga",
                                    "ppm", "heic", "avif", "mlt", "xml", "kdenlive"};
    const QFileInfo fi(resource);
    return fi.isFile() && !stills.contains(fi.suffix().toLower());
}

} // namespace

std::unique_ptr<Mlt::Producer> ProducerFactory::open(const QString& resource)
{
    std::unique_ptr<Mlt::Producer> p;
    if (forOwnDecoder(resource)) p = Video::openProducer(m_base, resource, {m_threads, m_parallelFrames, m_preview});
    if (!p) {
        p = std::make_unique<Mlt::Producer>(m_base, resource.toUtf8().constData());
        if (!p->is_valid()) return p;
        if (m_threads > 0) p->set("threads", m_threads);
    }
    // The color standard of the video stream is known right after opening (meta.media.colorspace only after the first frame)
    const int vi = p->get_int("video_index");
    const int cs = vi >= 0 ? p->get_int(QString("meta.media.%1.codec.colorspace").arg(vi).toUtf8().constData()) : 0;
    // Only standards MLT knows when scaling (601 family, 240); it does not handle 2020 etc. anyway
    const bool known = cs == 601 || cs == 170 || cs == 470 || cs == 624 || cs == 240;
    if (!known || cs == m_base.colorspace()) return p;
    std::lock_guard lock(m_mutex);
    auto& prof = m_profiles[cs];
    if (!prof) {
        prof = std::make_unique<Mlt::Profile>();
        prof->set_width(m_base.width());
        prof->set_height(m_base.height());
        prof->set_frame_rate(m_base.frame_rate_num(), m_base.frame_rate_den());
        prof->set_sample_aspect(m_base.sample_aspect_num(), m_base.sample_aspect_den());
        prof->set_display_aspect(m_base.display_aspect_num(), m_base.display_aspect_den());
        prof->set_progressive(m_base.progressive());
        prof->set_explicit(1);
        prof->set_colorspace(cs);
    }
    // Move the producer and the normalizing filters the loader attached (swscale …) to that profile instead of opening
    // the file a second time (each open probes all streams, ~100 ms with several audio streams: slow export start)
    p->set_profile(*prof);
    for (int i = 0; i < p->filter_count(); ++i)
        if (std::unique_ptr<Mlt::Filter> f{p->filter(i)}; f && f->is_valid()) f->set_profile(*prof);
    return p;
}

ClipFormat detectClipFormat(const QString& path)
{
    ClipFormat cf;
    // Read with the own decoder (stream description only, nothing decoded) as the preview reads the file
    if (const std::optional<MediaInfo> m = MediaProbe::read(path, FrameRate{25, 1}); m && m->isImage)
        return cf; // still images have no format of their own
    if (MediaProbe::isAudioFile(path)) return cf;
    Video::Reader::Options options;
    options.hardware = false;
    const std::unique_ptr<Video::Reader> reader = Video::Reader::open(path, options);
    if (!reader) return cf;
    const Video::Info& in = reader->info();
    int w = in.width, h = in.height;
    const double sar = av_q2d(in.sar);
    if (sar > 0 && std::abs(sar - 1.0) > 0.01) w = int(std::lround(w * sar)); // anamorphic pixels
    if (int(std::lround(in.rotation)) % 180 == 90) std::swap(w, h); // phone portrait: file landscape, rotation in the metadata
    const int num = in.frameRate.num, den = in.frameRate.den;
    if (w <= 0 || h <= 0 || num <= 0 || den <= 0) return cf;
    // The average frame rate (the stream's rate above is the highest one with variable frame rate)
    const AVRational avg = reader->container()->streams[in.index]->avg_frame_rate;

    cf.ok = true;
    cf.width = w & ~1; // even dimensions (yuv420p)
    cf.height = h & ~1;
    cf.rate = {num, den};
    cf.averageFps = avg.num > 0 && avg.den > 0 ? av_q2d(avg) : 0.0;
    // If the average differs noticeably the frame rate is variable (phones, game capture that skips frames).
    // The nominal rate is what the recording aimed for (DVR clips: nominal 60 or 120, average ~45) -> nearest
    // 24/25/30/50/60 of it; only a nominal rate that is a time base rather than a rate (mkv/webm 1000, 90000)
    // falls back to the average.
    cf.variable = cf.averageFps > 0 && std::abs(cf.averageFps - cf.rate.fps()) > 0.01 * cf.rate.fps();
    cf.suggested = !cf.variable ? nearestFrameRate(cf.rate.fps())
                   : cf.rate.fps() <= 240 ? nearestFrameRate(std::min(cf.rate.fps(), 60.0), true)
                                          : nearestFrameRate(cf.averageFps, true);
    return cf;
}
