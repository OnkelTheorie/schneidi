#include "engine/Exporter.h"

#include "core/TimelineOps.h"
#include "engine/TimelineBuilder.h"

#include <Mlt.h>
#include <QFile>

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
    m_length = TimelineOps::endFrame(tl);
    if (m_length <= 0) {
        if (error) *error = "Die Timeline ist leer.";
        return false;
    }

    m_profile = std::make_unique<Mlt::Profile>(s.profile.toUtf8().constData());
    m_builder = std::make_unique<TimelineBuilder>(*m_profile);
    m_tractor = m_builder->build(tl);
    m_tractor->set_in_and_out(0, m_length - 1);

    m_consumer = std::make_unique<Mlt::Consumer>(*m_profile, "avformat", s.path.toUtf8().constData());
    if (!m_consumer->is_valid()) {
        if (error) *error = "FFmpeg-Ausgabe (avformat) nicht verfügbar.";
        cleanup();
        return false;
    }
    m_consumer->set("vcodec", s.videoCodec.toUtf8().constData());
    m_consumer->set("acodec", s.audioCodec.toUtf8().constData());
    m_consumer->set("crf", s.crf);
    m_consumer->set("preset", s.preset.toUtf8().constData());
    m_consumer->set("ab", QString("%1k").arg(s.audioBitrateK).toUtf8().constData());
    m_consumer->set("pix_fmt", "yuv420p");
    m_consumer->set("movflags", "+faststart");
    m_consumer->set("real_time", -1);         // jedes Frame rendern (1 Thread: Effekte wie frei0r sind nicht thread-sicher)
    m_consumer->set("terminate_on_pause", 1); // am Ende automatisch stoppen
    m_consumer->connect(*m_tractor);

    m_path = s.path;
    m_tractor->set_speed(1);
    m_tractor->seek(0);
    if (m_consumer->start() != 0) {
        if (error) *error = "Export konnte nicht gestartet werden.";
        cleanup();
        return false;
    }
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
        emit progress(100);
        emit finished(true, QString("Export fertig: %1").arg(path));
        return;
    }
    const int pos = m_tractor->position();
    emit progress(std::clamp(pos * 100 / std::max(1, m_length), 0, 99));
}

void Exporter::cancel()
{
    if (!m_consumer) return;
    m_consumer->stop();
    const QString path = m_path;
    cleanup();
    QFile::remove(path); // halbfertige Datei weg
    emit finished(false, "Export abgebrochen.");
}

void Exporter::cleanup()
{
    m_timer.stop();
    m_consumer.reset();
    m_tractor.reset();
    m_builder.reset();
    m_profile.reset();
}
