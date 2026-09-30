#include "engine/Exporter.h"

#include "core/EffectRegistry.h"
#include "core/I18n.h"
#include "core/Subtitles.h"
#include "core/TimelineOps.h"
#include "engine/Profiles.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSettings>
#include <QThread>
#include <algorithm>
#include <atomic>

namespace {
std::atomic<int> g_running{0}; // laufende Exporte (m_timer läuft genau dann)

// Sichtbare Untertitelspur (sonst die erste mit Einträgen) im Bereich [from, to) als SRT, Zeiten ab Bereichsanfang
bool writeSubtitleFile(const Timeline& tl, double fps, int from, int to, const QString& path, QString* error)
{
    const SubtitleTrack* track = nullptr;
    for (const SubtitleTrack& t : tl.subtitles)
        if (t.enabled && !t.cues.isEmpty()) track = &t;
    for (const SubtitleTrack& t : tl.subtitles)
        if (!track && !t.cues.isEmpty()) track = &t;
    if (!track) {
        if (error) *error = T("Die Timeline enthält keine Untertitel.");
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(Subtitles::toSrt(track->cues, fps, from, to)) < 0 || !f.commit()) {
        if (error) *error = T("Untertiteldatei kann nicht geschrieben werden: %1").arg(path);
        return false;
    }
    return true;
}

bool usesFrei0r(const Timeline& tl)
{
    for (const Track& t : tl.video)
        for (const Clip& c : t.clips)
            for (const EffectInstance& e : c.effects) {
                const EffectDescriptor* d = EffectRegistry::find(e.effectId);
                if (e.enabled && d && d->mltService.startsWith("frei0r.")) return true;
            }
    return false;
}

} // namespace

bool Exporter::anyRunning()
{
    return g_running > 0;
}

int Exporter::savedCores()
{
    return std::max(0, QSettings().value("render/cores", 0).toInt());
}

void Exporter::setSavedCores(int cores)
{
    QSettings().setValue("render/cores", std::max(0, cores));
}

int Exporter::availableCores()
{
    return std::max(1, QThread::idealThreadCount());
}

int Exporter::parallelFrames(const Timeline& tl, int cores)
{
    bool frei0r = usesFrei0r(tl);
    if (tl.nested)
        for (const Timeline& n : *tl.nested) frei0r = frei0r || usesFrei0r(n);
    return frei0r ? 1 : (cores > 0 ? cores : availableCores());
}
#include <QFileInfo>

Exporter::Exporter(QObject* parent) : QObject(parent)
{
    m_timer.setInterval(200);
    connect(&m_timer, &QTimer::timeout, this, &Exporter::poll);
}

Exporter::~Exporter()
{
    cancel();
}

bool Exporter::start(const Timeline& tl, const ExportSettings& s, QString* error)
{
    if (isRunning()) return false;
    const int end = TimelineOps::endFrame(tl);
    m_from = std::clamp(s.from, 0, std::max(0, end - 1));
    const int to = s.to < 0 ? end - 1 : std::min(s.to, end - 1);
    m_length = to - m_from + 1;
    if (end <= 0 || m_length <= 0) {
        if (error) *error = end <= 0 ? T("Die Timeline ist leer.") : T("Der In/Out-Bereich enthält nichts.");
        return false;
    }

    // Andere Ausgabegröße als die Timeline: Pixelwerte (Position, Titel …) mitskalieren
    ProjectFormat out = s.format;
    Timeline scaled = tl;
    if (!s.size.isEmpty() && s.size != s.format.size()) {
        out.width = s.size.width() & ~1;
        out.height = s.size.height() & ~1;
        scaleTimeline(scaled, s.format.size(), out.size());
    }
    m_profile = makeProfile(out);
    m_builder = std::make_unique<TimelineBuilder>(*m_profile);
    const int cores = s.cores > 0 ? s.cores : availableCores();
    m_builder->setDecoderThreads(cores);
    m_builder->setSubtitles(s.burnSubtitles && !s.videoCodec.isEmpty());
    m_tractor = m_builder->build(scaled);
    m_tractor->set_in_and_out(m_from, m_from + m_length - 1);

    QDir().mkpath(QFileInfo(s.path).absolutePath());
    if (!s.subtitlePath.isEmpty() && !writeSubtitleFile(tl, s.format.rate.fps(), m_from, m_from + m_length, s.subtitlePath, error)) {
        cleanup();
        return false;
    }
    m_consumer = std::make_unique<Mlt::Consumer>(*m_profile, "avformat", s.path.toUtf8().constData());
    if (!m_consumer->is_valid()) {
        if (error) *error = T("FFmpeg-Ausgabe (avformat) nicht verfügbar.");
        cleanup();
        return false;
    }
    if (s.videoCodec.isEmpty()) {
        m_consumer->set("vn", 1); // nur Audio
    } else {
        m_consumer->set("vcodec", s.videoCodec.toUtf8().constData());
        m_consumer->set("pix_fmt", s.pixFmt.toUtf8().constData());
        if (s.videoCodec == "libx264" || s.videoCodec == "libx265") {
            m_consumer->set("crf", s.crf);
            m_consumer->set("preset", s.preset.toUtf8().constData());
        }
    }
    m_consumer->set("acodec", s.audioCodec.toUtf8().constData());
    if (!s.audioCodec.startsWith("pcm_")) m_consumer->set("ab", QString("%1k").arg(s.audioBitrateK).toUtf8().constData());
    const QString ext = QFileInfo(s.path).suffix().toLower();
    if (ext == "mp4" || ext == "mov" || ext == "m4a") m_consumer->set("movflags", "+faststart");
    // Jedes Frame rendern (real_time < 0), mehrere Bilder gleichzeitig (außer mit frei0r, siehe parallelFrames);
    // Decoder (oben) und Encoder bekommen dieselbe Kernzahl
    const int frames = parallelFrames(tl, cores);
    m_consumer->set("real_time", -frames);
    m_consumer->set("threads", cores);
    qInfo("Export: %d Kerne, %d Bilder gleichzeitig", cores, frames);
    m_consumer->set("terminate_on_pause", 1); // am Ende automatisch stoppen
    m_consumer->connect(*m_tractor);

    m_path = s.path;
    m_tractor->set_speed(1);
    m_tractor->seek(0); // Position zählt ab dem In-Punkt (seek(m_from) ließ bei In/Out die ersten m_from Frames weg)
    if (m_consumer->start() != 0) {
        if (error) *error = T("Export konnte nicht gestartet werden.");
        cleanup();
        return false;
    }
    ++g_running;
    m_timer.start();
    emit progress(0);
    return true;
}

void Exporter::poll()
{
    if (!m_consumer) return;
    if (m_consumer->is_stopped()) {
        const QString path = m_path;
        cleanup();
        // Encoder/Container gescheitert (z. B. Codec fehlt): keine oder leere Datei
        if (QFileInfo(path).size() <= 0) {
            QFile::remove(path);
            emit finished(false, T("Export fehlgeschlagen: %1").arg(path));
            return;
        }
        emit progress(100);
        emit finished(true, T("Export fertig: %1").arg(path));
        return;
    }
    const int pos = m_tractor->position(); // relativ zum In-Punkt
    emit progress(std::clamp(pos * 100 / std::max(1, m_length), 0, 99));
}

void Exporter::cancel()
{
    if (!m_consumer) return;
    m_consumer->stop();
    const QString path = m_path;
    cleanup();
    QFile::remove(path); // halbfertige Datei weg
    emit finished(false, T("Export abgebrochen."));
}

void Exporter::cleanup()
{
    if (m_timer.isActive()) --g_running;
    m_timer.stop();
    m_consumer.reset();
    m_tractor.reset();
    m_builder.reset();
    m_profile.reset();
}
