#include "engine/Profiles.h"
#include "engine/Bundle.h"

#include <Mlt.h>
#include <QByteArray>
#include <QFileInfo>
#include <QProcess>
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

std::unique_ptr<Mlt::Producer> ProducerFactory::open(const QString& resource)
{
    const QByteArray res = resource.toUtf8();
    auto p = std::make_unique<Mlt::Producer>(m_base, res.constData());
    if (!p->is_valid()) return p;
    if (m_threads > 0) p->set("threads", m_threads);
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

namespace {

double parseRate(const QByteArray& s)
{
    const QList<QByteArray> parts = s.trimmed().split('/');
    const double num = parts.value(0).toDouble();
    const double den = parts.size() > 1 ? parts[1].toDouble() : 1.0;
    return den > 0 ? num / den : 0.0;
}

// Average frame rate via ffprobe (MLT only knows r_frame_rate, i.e. the highest with variable frame rate).
// Without ffprobe: 0 -> only the value from MLT counts.
double averageFps(const QString& path)
{
    QProcess proc;
    proc.start(Bundle::tool("ffprobe"), {"-v", "error", "-select_streams", "v:0", "-show_entries", "stream=avg_frame_rate",
                           "-of", "default=noprint_wrappers=1:nokey=1", path});
    if (!proc.waitForFinished(5000)) {
        proc.kill();
        proc.waitForFinished(500);
        return 0;
    }
    return proc.exitCode() == 0 ? parseRate(proc.readAllStandardOutput()) : 0.0;
}

} // namespace

ClipFormat detectClipFormat(const QString& path)
{
    ClipFormat cf;
    Mlt::Profile profile("atsc_1080p_25"); // only for opening
    Mlt::Producer p(profile, path.toUtf8().constData());
    if (!p.is_valid()) return cf;
    const QByteArray svc = p.get("mlt_service");
    if (svc == "qimage" || svc == "pixbuf") return cf; // still images have no format of their own
    static const QStringList audioExt{"mp3", "wav", "flac", "ogg", "opus", "m4a", "aac", "wma", "aiff"};
    const int vi = p.get_int("video_index");
    if (vi < 0 || audioExt.contains(QFileInfo(path).suffix().toLower())) return cf;

    // MLT only sets meta.media.* after the first frame
    std::unique_ptr<Mlt::Frame> frame(p.get_frame());
    int w = p.get_int("meta.media.width"), h = p.get_int("meta.media.height");
    const double sar = p.get_double("aspect_ratio");
    if (sar > 0 && std::abs(sar - 1.0) > 0.01) w = int(std::lround(w * sar)); // anamorphic pixels
    const int rotate = std::abs(p.get_int(QString("meta.media.%1.codec.rotate").arg(vi).toUtf8().constData())) % 180;
    if (rotate == 90) std::swap(w, h); // phone portrait: file landscape, with rotation in the metadata
    const int num = p.get_int("meta.media.frame_rate_num"), den = p.get_int("meta.media.frame_rate_den");
    if (w <= 0 || h <= 0 || num <= 0 || den <= 0) return cf;

    cf.ok = true;
    cf.width = w & ~1; // even dimensions (yuv420p)
    cf.height = h & ~1;
    cf.rate = {num, den};
    cf.averageFps = averageFps(path);
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
