#include "engine/Profiles.h"

#include <Mlt.h>
#include <QByteArray>
#include <QFileInfo>
#include <QProcess>
#include <numeric>

std::unique_ptr<Mlt::Profile> makeProfile(const ProjectFormat& f)
{
    auto p = std::make_unique<Mlt::Profile>("atsc_1080p_25"); // Grundlage, alle Werte werden überschrieben
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
    // Farbnorm des Videostroms steht schon nach dem Öffnen fest (meta.media.colorspace erst nach dem ersten Frame)
    const int vi = p->get_int("video_index");
    const int cs = vi >= 0 ? p->get_int(QString("meta.media.%1.codec.colorspace").arg(vi).toUtf8().constData()) : 0;
    // Nur Normen, die MLT beim Skalieren kennt (601-Familie, 240); 2020 u. Ä. behandelt es ohnehin nicht
    const bool known = cs == 601 || cs == 170 || cs == 470 || cs == 624 || cs == 240;
    if (!known || cs == m_base.colorspace()) return p;
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
    auto own = std::make_unique<Mlt::Producer>(*prof, res.constData());
    return own->is_valid() ? std::move(own) : std::move(p);
}

namespace {

double parseRate(const QByteArray& s)
{
    const QList<QByteArray> parts = s.trimmed().split('/');
    const double num = parts.value(0).toDouble();
    const double den = parts.size() > 1 ? parts[1].toDouble() : 1.0;
    return den > 0 ? num / den : 0.0;
}

// Mittlere Framerate per ffprobe (MLT kennt nur r_frame_rate, bei variabler Framerate also die höchste).
// Ohne ffprobe: 0 -> nur die Angabe aus MLT zählt.
double averageFps(const QString& path)
{
    QProcess proc;
    proc.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_entries", "stream=avg_frame_rate",
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
    Mlt::Profile profile("atsc_1080p_25"); // nur zum Öffnen
    Mlt::Producer p(profile, path.toUtf8().constData());
    if (!p.is_valid()) return cf;
    const QByteArray svc = p.get("mlt_service");
    if (svc == "qimage" || svc == "pixbuf") return cf; // Standbilder haben kein eigenes Format
    static const QStringList audioExt{"mp3", "wav", "flac", "ogg", "opus", "m4a", "aac", "wma", "aiff"};
    const int vi = p.get_int("video_index");
    if (vi < 0 || audioExt.contains(QFileInfo(path).suffix().toLower())) return cf;

    // meta.media.* setzt MLT erst nach dem ersten Frame
    std::unique_ptr<Mlt::Frame> frame(p.get_frame());
    int w = p.get_int("meta.media.width"), h = p.get_int("meta.media.height");
    const double sar = p.get_double("aspect_ratio");
    if (sar > 0 && std::abs(sar - 1.0) > 0.01) w = int(std::lround(w * sar)); // anamorphe Pixel
    const int rotate = std::abs(p.get_int(QString("meta.media.%1.codec.rotate").arg(vi).toUtf8().constData())) % 180;
    if (rotate == 90) std::swap(w, h); // Handy hochkant: Datei quer, mit Drehung in den Metadaten
    const int num = p.get_int("meta.media.frame_rate_num"), den = p.get_int("meta.media.frame_rate_den");
    if (w <= 0 || h <= 0 || num <= 0 || den <= 0) return cf;

    cf.ok = true;
    cf.width = w & ~1; // gerade Maße (yuv420p)
    cf.height = h & ~1;
    cf.rate = {num, den};
    cf.averageFps = averageFps(path);
    // Weicht der Mittelwert merklich ab, ist die Framerate variabel (Handy) -> nominelle 24/25/30/50/60
    cf.variable = cf.averageFps > 0 && std::abs(cf.averageFps - cf.rate.fps()) > 0.01 * cf.rate.fps();
    cf.suggested = cf.variable ? nearestFrameRate(cf.averageFps, true) : nearestFrameRate(cf.rate.fps());
    return cf;
}
